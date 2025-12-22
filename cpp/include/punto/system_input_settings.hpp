/**
 * @file system_input_settings.hpp
 * @brief Чтение/запись системных настроек хоткея переключения раскладки
 */

#pragma once

#include <optional>
#include <string>

namespace punto {

/// Комбинация "модификатор + клавиша" в терминах config.yaml (leftctrl, space и т.д.)
struct LayoutToggle {
  std::string modifier;
  std::string key;
};

enum class SystemInputResult {
  Ok,
  NotAvailable,
  Unsupported,
  Error,
};

struct SystemInputReadOutcome {
  SystemInputResult result = SystemInputResult::Error;

  /// "gnome" или "x11" (если определено)
  std::string backend;

  /// Сырые данные из системы (accel string / XKB option)
  std::string raw;

  /// Разобранная комбинация (если представима)
  std::optional<LayoutToggle> toggle;

  /// Текст ошибки (если result != Ok)
  std::string error;
};

struct SystemInputWriteOutcome {
  SystemInputResult result = SystemInputResult::Error;
  std::string backend;
  std::string error;
};

class SystemInputSettings {
public:
  /// Читает текущий системный хоткей переключения раскладки.
  [[nodiscard]] static SystemInputReadOutcome read_layout_toggle();

  /// Проверяет, можно ли применить хоткей в текущем backend (без изменения системы).
  [[nodiscard]] static SystemInputWriteOutcome validate_layout_toggle(const LayoutToggle& toggle);

  /// Пишет хоткей переключения раскладки в систему (GNOME или X11 backend).
  [[nodiscard]] static SystemInputWriteOutcome write_layout_toggle(const LayoutToggle& toggle);
};

} // namespace punto
