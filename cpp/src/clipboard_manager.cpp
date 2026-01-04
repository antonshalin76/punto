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
#include <vector>

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

  // Часто используемые targets (для SelectionRequest).
  atom_targets_ = XInternAtom(display_, "TARGETS", False);
  atom_text_plain_ = XInternAtom(display_, "text/plain", False);
  atom_text_plain_utf8_ = XInternAtom(display_, "text/plain;charset=utf-8", False);

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

void ClipboardManager::pump_events() {
  if (!display_) {
    return;
  }

  // Важно: обрабатываем только selection-события. Остальные события нам не нужны.
  XEvent ev;

  while (XCheckTypedEvent(display_, SelectionRequest, &ev) != 0) {
    handle_selection_request(ev.xselectionrequest);
  }

  while (XCheckTypedEvent(display_, SelectionClear, &ev) != 0) {
    handle_selection_clear(ev.xselectionclear);
  }
}

void ClipboardManager::handle_selection_clear(const XSelectionClearEvent &ev) {
  if (ev.selection == atom_clipboard_) {
    owns_clipboard_ = false;
    clipboard_text_.clear();
    return;
  }

  if (ev.selection == atom_primary_) {
    owns_primary_ = false;
    primary_text_.clear();
    return;
  }
}

void ClipboardManager::handle_selection_request(const XSelectionRequestEvent &req) {
  if (!display_) {
    return;
  }

  const std::string *payload = nullptr;

  if (req.selection == atom_clipboard_ && owns_clipboard_) {
    payload = &clipboard_text_;
  } else if (req.selection == atom_primary_ && owns_primary_) {
    payload = &primary_text_;
  }

  // По ICCCM request.property может быть None.
  Atom property = req.property;
  if (property == None) {
    property = req.target;
  }

  XEvent reply{};
  reply.xselection.type = SelectionNotify;
  reply.xselection.display = req.display;
  reply.xselection.requestor = req.requestor;
  reply.xselection.selection = req.selection;
  reply.xselection.target = req.target;
  reply.xselection.time = req.time;
  reply.xselection.property = None;

  if (payload == nullptr) {
    // Мы не владеем этим selection или у нас нет данных.
    (void)XSendEvent(display_, req.requestor, False, 0, &reply);
    XFlush(display_);
    return;
  }

  if (req.target == atom_targets_) {
    std::vector<Atom> targets;
    targets.reserve(6);
    targets.push_back(atom_targets_);
    targets.push_back(atom_utf8_string_);
    targets.push_back(atom_text_plain_utf8_);
    targets.push_back(atom_text_plain_);
    targets.push_back(XA_STRING);

    const int n = static_cast<int>(targets.size());

    XChangeProperty(display_, req.requestor, property,
                    /*type=*/XA_ATOM,
                    /*format=*/32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char *>(targets.data()), n);

    reply.xselection.property = property;

    (void)XSendEvent(display_, req.requestor, False, 0, &reply);
    XFlush(display_);
    return;
  }

  const bool is_text_target =
      (req.target == atom_utf8_string_ || req.target == atom_text_plain_utf8_ ||
       req.target == atom_text_plain_ || req.target == XA_STRING);

  if (!is_text_target) {
    // Unsupported target
    (void)XSendEvent(display_, req.requestor, False, 0, &reply);
    XFlush(display_);
    return;
  }

  // Передаём байты "как есть" (UTF-8). В современных приложениях target чаще всего
  // UTF8_STRING или text/plain;charset=utf-8.
  const int nbytes = static_cast<int>(payload->size());
  XChangeProperty(display_, req.requestor, property,
                  /*type=*/req.target,
                  /*format=*/8,
                  PropModeReplace,
                  reinterpret_cast<const unsigned char *>(payload->data()), nbytes);

  reply.xselection.property = property;

  (void)XSendEvent(display_, req.requestor, False, 0, &reply);
  XFlush(display_);
}

bool ClipboardManager::wait_for_selection_notify(Atom property) {
  auto start = std::chrono::steady_clock::now();
  XEvent event;

  while (std::chrono::steady_clock::now() - start < timeout_) {
    // Если мы владеем selection, то при XConvertSelection() сервер присылает нам
    // SelectionRequest, и без обработки этого события SelectionNotify не придёт.
    pump_events();

    if (XCheckTypedWindowEvent(display_, window_, SelectionNotify, &event) != 0) {
      if (event.xselection.property == property) {
        return event.xselection.property != None;
      }
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

  if (!wait_for_selection_notify(property)) {
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
    if (!open()) {
      return ClipboardResult::NoConnection;
    }
  }

  const Atom selection = get_selection_atom(sel);

  // Сохраняем данные в памяти процесса: в X11 selection "данные" находятся у owner.
  if (sel == Selection::Clipboard) {
    clipboard_text_.assign(text.data(), text.size());
    owns_clipboard_ = true;
  } else {
    primary_text_.assign(text.data(), text.size());
    owns_primary_ = true;
  }

  XSetSelectionOwner(display_, selection, window_, CurrentTime);
  XFlush(display_);

  const Window owner = XGetSelectionOwner(display_, selection);
  if (owner != window_) {
    if (sel == Selection::Clipboard) {
      owns_clipboard_ = false;
      clipboard_text_.clear();
    } else {
      owns_primary_ = false;
      primary_text_.clear();
    }
    return ClipboardResult::ConversionFailed;
  }

  return ClipboardResult::Ok;
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
