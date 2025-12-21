/**
 * @file x11_session.cpp
 * @brief Реализация управления X11 сессией
 */

#include "punto/x11_session.hpp"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <pwd.h>
#include <unistd.h>
#include <unordered_map>

namespace punto {

namespace {

/// Выполняет команду и возвращает stdout
std::string exec_command(const std::string &cmd) {
  std::array<char, 256> buffer{};
  std::string result;

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    return "";

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    result += buffer.data();
  }
  pclose(pipe);

  // Удаляем trailing newline
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }

  return result;
}

/// Читает переменные окружения из /proc/<pid>/environ
std::unordered_map<std::string, std::string>
read_proc_environ(const std::string &pid) {
  std::unordered_map<std::string, std::string> env;

  std::ifstream file("/proc/" + pid + "/environ", std::ios::binary);
  if (!file)
    return env;

  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

  std::size_t start = 0;
  while (start < content.size()) {
    std::size_t end = content.find('\0', start);
    if (end == std::string::npos)
      break;

    std::string_view entry(content.data() + start, end - start);
    auto eq_pos = entry.find('=');
    if (eq_pos != std::string_view::npos) {
      std::string key(entry.substr(0, eq_pos));
      std::string value(entry.substr(eq_pos + 1));
      env[std::move(key)] = std::move(value);
    }

    start = end + 1;
  }

  return env;
}

} // namespace

bool X11Session::initialize() {
  if (initialized_)
    return true;

  // Сохраняем оригинальные effective UID/GID
  original_uid_ = geteuid();
  original_gid_ = getegid();

  // Находим активного пользователя
  auto username = find_active_user();
  if (!username) {
    std::cerr
        << "[punto] Ошибка: не удалось найти активного GUI пользователя\n";
    return false;
  }

  info_.username = *username;

  // Получаем информацию о пользователе
  struct passwd *pw = getpwnam(info_.username.c_str());
  if (!pw) {
    std::cerr << "[punto] Ошибка: пользователь " << info_.username
              << " не найден в системе\n";
    return false;
  }

  info_.uid = pw->pw_uid;
  info_.gid = pw->pw_gid;
  info_.home_dir = pw->pw_dir;
  info_.xdg_runtime_dir = "/run/user/" + std::to_string(info_.uid);

  // Находим DISPLAY/XAUTHORITY
  if (!find_session_env(info_.username)) {
    // Устанавливаем дефолты
    info_.display = ":0";
    info_.xauthority_path =
        "/run/user/" + std::to_string(info_.uid) + "/gdm/Xauthority";
  }

  initialized_ = true;
  return true;
}

bool X11Session::is_valid() const noexcept { return initialized_; }

const X11SessionInfo &X11Session::info() const noexcept { return info_; }

void X11Session::apply_environment() const {
  if (!initialized_)
    return;

  setenv("DISPLAY", info_.display.c_str(), 1);
  setenv("XAUTHORITY", info_.xauthority_path.c_str(), 1);
  setenv("HOME", info_.home_dir.c_str(), 1);
  setenv("USER", info_.username.c_str(), 1);
  setenv("LOGNAME", info_.username.c_str(), 1);
  setenv("XDG_RUNTIME_DIR", info_.xdg_runtime_dir.c_str(), 1);
}

bool X11Session::switch_to_user() const {
  if (!initialized_)
    return false;
  if (geteuid() == static_cast<uid_t>(info_.uid))
    return true; // Уже этот пользователь

  // Сначала gid, потом uid
  if (setegid(static_cast<gid_t>(info_.gid)) != 0) {
    return false;
  }
  if (seteuid(static_cast<uid_t>(info_.uid)) != 0) {
    // Откатываем gid (best effort, ignoring errors)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    setegid(original_gid_);
#pragma GCC diagnostic pop
    return false;
  }

  return true;
}

bool X11Session::switch_to_root() const {
  if (geteuid() == 0)
    return true; // Уже root

  // Сначала gid, потом uid
  if (setegid(original_gid_) != 0) {
    return false;
  }
  if (seteuid(original_uid_) != 0) {
    return false;
  }

  return true;
}

std::optional<std::string> X11Session::find_active_user() {
  // Метод 1: loginctl (systemd)
  std::string user = exec_command("loginctl list-sessions 2>/dev/null | grep "
                                  "'seat0' | awk '{print $3}' | head -n 1");

  if (!user.empty()) {
    return user;
  }

  // Метод 2: who
  user = exec_command(
      "who 2>/dev/null | grep '(:0)' | awk '{print $1}' | head -n 1");
  if (!user.empty()) {
    return user;
  }

  // Метод 3: владелец /dev/tty1 или консоли
  user = exec_command("stat -c '%U' /dev/tty1 2>/dev/null");
  if (!user.empty() && user != "root") {
    return user;
  }

  return std::nullopt;
}

bool X11Session::find_session_env(const std::string &username) {
  // Ищем процесс gnome-session или kde пользователя
  std::string pid =
      exec_command("pgrep -u " + username +
                   " 'gnome-session|plasma' 2>/dev/null | head -n 1");

  if (pid.empty()) {
    // Fallback: ищем любой X-клиент
    pid = exec_command("pgrep -u " + username +
                       " -f '^/usr/bin/gnome-shell' 2>/dev/null | head -n 1");
  }

  if (pid.empty()) {
    return false;
  }

  auto env = read_proc_environ(pid);

  if (auto it = env.find("DISPLAY"); it != env.end()) {
    info_.display = it->second;
  }

  if (auto it = env.find("XAUTHORITY"); it != env.end()) {
    info_.xauthority_path = it->second;
  }

  if (auto it = env.find("XDG_RUNTIME_DIR"); it != env.end()) {
    info_.xdg_runtime_dir = it->second;
  }

  return !info_.display.empty();
}

int X11Session::get_current_keyboard_layout() const {
  if (!initialized_)
    return -1;

  // Переходим в контекст пользователя
  if (!switch_to_user())
    return -1;
  apply_environment();

  Display *display = XOpenDisplay(nullptr);
  if (!display) {
    const char *display_name = std::getenv("DISPLAY");
    if (display_name) {
      display = XOpenDisplay(display_name);
    }
  }

  int group = -1;
  if (display) {
    XkbStateRec state;
    if (XkbGetState(display, XkbUseCoreKbd, &state) == Success) {
      group = state.group;
    }
    XCloseDisplay(display);
  }

  switch_to_root();
  return group;
}

bool X11Session::set_keyboard_layout(int index) const {
  if (!initialized_)
    return false;

  if (!switch_to_user())
    return false;
  apply_environment();

  Display *display = XOpenDisplay(nullptr);
  if (!display) {
    const char *display_name = std::getenv("DISPLAY");
    if (display_name) {
      display = XOpenDisplay(display_name);
    }
  }

  bool success = false;
  if (display) {
    XkbLockGroup(display, XkbUseCoreKbd, static_cast<unsigned int>(index));
    XSync(display, False);
    XCloseDisplay(display);
    success = true;
  }

  switch_to_root();
  return success;
}

} // namespace punto
