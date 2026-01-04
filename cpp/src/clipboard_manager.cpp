/**
 * @file clipboard_manager.cpp
 * @brief Реализация нативного X11 менеджера буфера обмена
 */

#include "punto/clipboard_manager.hpp"
#include "punto/terminal_detection.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace punto {

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

  // Получаем WM_CLASS (instance/class)
  //
  // Важно: иногда _NET_ACTIVE_WINDOW может указывать на дочернее окно.
  // Тогда WM_CLASS хранится на родителе. Делаем небольшой подъём по дереву.
  std::string wm_class;
  std::string wm_instance;

  Window w = active_window;
  for (int depth = 0; depth < 8 && w != None; ++depth) {
    XClassHint class_hint;
    if (XGetClassHint(display_, w, &class_hint) != 0) {
      if (class_hint.res_class) {
        wm_class = class_hint.res_class;
        XFree(class_hint.res_class);
      }
      if (class_hint.res_name) {
        wm_instance = class_hint.res_name;
        XFree(class_hint.res_name);
      }

      // Если нашли хоть что-то — достаточно.
      if (!wm_class.empty() || !wm_instance.empty()) {
        break;
      }
    }

    // Поднимаемся к родителю.
    Window root_ret = None;
    Window parent_ret = None;
    Window *children = nullptr;
    unsigned int nchildren = 0;

    if (XQueryTree(display_, w, &root_ret, &parent_ret, &children, &nchildren) == 0) {
      break;
    }
    if (children) {
      XFree(children);
    }

    if (parent_ret == None || parent_ret == w) {
      break;
    }
    w = parent_ret;
  }

  return is_terminal_wm_class(wm_instance, wm_class);
}

} // namespace punto
