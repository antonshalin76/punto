/**
 * @file clipboard_manager.cpp
 * @brief Реализация нативного X11 менеджера буфера обмена
 */

#include "punto/clipboard_manager.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace punto {

namespace {

/// Список терминальных эмуляторов
constexpr std::array kTerminalClasses = {"gnome-terminal",
                                         "gnome-terminal-server",
                                         "konsole",
                                         "xterm",
                                         "urxvt",
                                         "terminator",
                                         "tilix",
                                         "alacritty",
                                         "kitty",
                                         "terminology",
                                         "xfce4-terminal",
                                         "mate-terminal",
                                         "lxterminal",
                                         "qterminal",
                                         "sakura",
                                         "termite",
                                         "st",
                                         "foot"};

/// Проверяет, содержит ли строка подстроку (case insensitive)
bool contains_ci(const std::string &haystack, std::string_view needle) {
  if (needle.empty())
    return true;
  if (haystack.size() < needle.size())
    return false;

  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(),
                        needle.end(), [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                        });
  return it != haystack.end();
}

} // namespace

ClipboardManager::ClipboardManager(X11Session &session,
                                   std::chrono::milliseconds timeout)
    : session_{session}, timeout_{timeout} {}

ClipboardManager::~ClipboardManager() { close(); }

bool ClipboardManager::open() {
  if (display_)
    return true;

  // Применяем переменные окружения X11
  session_.apply_environment();

  display_ = XOpenDisplay(nullptr);
  if (!display_) {
    std::cerr << "[punto] Ошибка: не удалось открыть X display\n";
    return false;
  }

  // Создаём скрытое окно для работы с selections
  int screen = DefaultScreen(display_);
  window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen), 0, 0, 1,
                                1, 0, BlackPixel(display_, screen),
                                WhitePixel(display_, screen));

  // Кэшируем атомы
  atom_clipboard_ = XInternAtom(display_, "CLIPBOARD", False);
  atom_primary_ = XInternAtom(display_, "PRIMARY", False);
  atom_utf8_string_ = XInternAtom(display_, "UTF8_STRING", False);
  atom_targets_ = XInternAtom(display_, "TARGETS", False);

  return true;
}

void ClipboardManager::close() {
  if (display_) {
    if (window_ != None) {
      XDestroyWindow(display_, window_);
      window_ = None;
    }
    XCloseDisplay(display_);
    display_ = nullptr;
  }
}

bool ClipboardManager::is_open() const noexcept { return display_ != nullptr; }

Atom ClipboardManager::get_selection_atom(Selection sel) const {
  return sel == Selection::Primary ? atom_primary_ : atom_clipboard_;
}

bool ClipboardManager::wait_for_selection_notify(Atom selection) {
  auto start = std::chrono::steady_clock::now();
  XEvent event;

  while (std::chrono::steady_clock::now() - start < timeout_) {
    if (XCheckTypedWindowEvent(display_, window_, SelectionNotify, &event)) {
      return event.xselection.property != None;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  return false;
}

std::optional<std::string> ClipboardManager::get_text(Selection sel) {
  if (!display_) {
    if (!open())
      return std::nullopt;
  }

  Atom selection = get_selection_atom(sel);
  Atom property = XInternAtom(display_, "PUNTO_SEL", False);

  // Запрашиваем конвертацию selection в UTF8_STRING
  XConvertSelection(display_, selection, atom_utf8_string_, property, window_,
                    CurrentTime);
  XFlush(display_);

  if (!wait_for_selection_notify(selection)) {
    return std::nullopt;
  }

  // Читаем результат
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *data = nullptr;

  int result = XGetWindowProperty(display_, window_, property, 0, 65536,
                                  True, // delete = True
                                  AnyPropertyType, &actual_type, &actual_format,
                                  &nitems, &bytes_after, &data);

  if (result != Success || data == nullptr) {
    return std::nullopt;
  }

  std::string text(reinterpret_cast<char *>(data), nitems);
  XFree(data);

  return text;
}

ClipboardResult ClipboardManager::set_text(Selection sel,
                                           std::string_view text) {
  if (!display_) {
    if (!open())
      return ClipboardResult::NoConnection;
  }

  // Для установки selection нам нужно стать его владельцем
  // и обрабатывать SelectionRequest события.
  // Это сложнее, чем просто установить — нужен event loop.
  //
  // Для простоты, используем xsel через exec (временное решение).
  // В Phase 3 можно реализовать полноценный ownership.

  // Временная реализация через pipe к xsel
  std::string cmd = "xsel --clipboard --input";
  if (sel == Selection::Primary) {
    cmd = "xsel --primary --input";
  }

  FILE *pipe = popen(cmd.c_str(), "w");
  if (!pipe) {
    return ClipboardResult::ConversionFailed;
  }

  fwrite(text.data(), 1, text.size(), pipe);
  int ret = pclose(pipe);

  return ret == 0 ? ClipboardResult::Ok : ClipboardResult::ConversionFailed;
}

bool ClipboardManager::is_active_window_terminal() {
  if (!display_) {
    if (!open())
      return false;
  }

  // Получаем активное окно
  Atom net_active_window = XInternAtom(display_, "_NET_ACTIVE_WINDOW", True);
  if (net_active_window == None)
    return false;

  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *data = nullptr;

  int root_screen = DefaultScreen(display_);
  Window root = RootWindow(display_, root_screen);

  int result = XGetWindowProperty(display_, root, net_active_window, 0, 1,
                                  False, XA_WINDOW, &actual_type,
                                  &actual_format, &nitems, &bytes_after, &data);

  if (result != Success || data == nullptr || nitems == 0) {
    if (data)
      XFree(data);
    return false;
  }

  Window active_window = *reinterpret_cast<Window *>(data);
  XFree(data);

  if (active_window == None)
    return false;

  // Получаем WM_CLASS
  XClassHint class_hint;
  if (XGetClassHint(display_, active_window, &class_hint) == 0) {
    return false;
  }

  std::string wm_class;
  if (class_hint.res_class) {
    wm_class = class_hint.res_class;
    XFree(class_hint.res_class);
  }
  if (class_hint.res_name) {
    XFree(class_hint.res_name);
  }

  // Проверяем, является ли это терминалом
  for (const auto &terminal : kTerminalClasses) {
    if (contains_ci(wm_class, terminal)) {
      return true;
    }
  }

  return false;
}

} // namespace punto
