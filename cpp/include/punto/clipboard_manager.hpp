/**
 * @file clipboard_manager.hpp
 * @brief Нативный менеджер буфера обмена X11
 *
 * Замена xclip/xsel — прямое взаимодействие с X11 selections.
 * Поддерживает CLIPBOARD и PRIMARY selections.
 */

#pragma once

#include <X11/Xlib.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "punto/types.hpp"
#include "punto/x11_session.hpp"

namespace punto {

/**
 * @brief Тип selection в X11
 */
enum class Selection {
  Primary,  // Автоматически заполняется при выделении текста
  Clipboard // Заполняется при Ctrl+C
};

/**
 * @brief Нативный менеджер буфера обмена X11
 *
 * Обеспечивает прямой доступ к X11 selections без вызова внешних утилит.
 * Использует RAII для управления X11 ресурсами.
 */
class ClipboardManager {
public:
  /**
   * @brief Конструктор
   * @param session X11 сессия для получения DISPLAY/XAUTHORITY
   * @param timeout Таймаут ожидания selection
   */
  explicit ClipboardManager(
      X11Session &session,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

  ~ClipboardManager();

  // Запрет копирования (X11 ресурсы)
  ClipboardManager(const ClipboardManager &) = delete;
  ClipboardManager &operator=(const ClipboardManager &) = delete;

  /**
   * @brief Открывает соединение с X сервером
   * @return true если соединение установлено
   */
  bool open();

  /**
   * @brief Закрывает соединение
   */
  void close();

  /**
   * @brief Проверяет, открыто ли соединение
   */
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * @brief Читает текст из selection
   * @param sel Тип selection (Primary или Clipboard)
   * @return Текст или nullopt при ошибке/таймауте
   */
  [[nodiscard]] std::optional<std::string> get_text(Selection sel);

  /**
   * @brief Записывает текст в selection
   * @param sel Тип selection
   * @param text Текст для записи
   * @return Результат операции
   */
  ClipboardResult set_text(Selection sel, std::string_view text);

  /**
   * @brief Определяет, является ли активное окно терминалом
   * @return true если активное окно — эмулятор терминала
   *
   * Используется для выбора правильных hotkeys (Ctrl+C vs Ctrl+Shift+C)
   */
  [[nodiscard]] bool is_active_window_terminal();

private:
  /**
   * @brief Атом selection по типу
   */
  Atom get_selection_atom(Selection sel) const;

  /**
   * @brief Ожидание SelectionNotify события
   */
  bool wait_for_selection_notify(Atom selection);

  X11Session &session_;
  std::chrono::milliseconds timeout_;

  Display *display_ = nullptr;
  Window window_ = None;

  // X11 атомы (кэшируются после открытия)
  Atom atom_clipboard_ = None;
  Atom atom_primary_ = None;
  Atom atom_utf8_string_ = None;
  Atom atom_targets_ = None;
};

} // namespace punto
