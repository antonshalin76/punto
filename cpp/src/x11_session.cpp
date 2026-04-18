/**
 * @file x11_session.cpp
 * @brief Реализация управления X11 сессией
 */

#include "punto/x11_session.hpp"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace punto {

namespace {

std::mutex &x11_open_mutex() {
  static std::mutex mu;
  return mu;
}

class ScopedEnvVar {
public:
  ScopedEnvVar(const char *name, const std::string &value) : name_{name} {
    const char *current = std::getenv(name_);
    if (current != nullptr) {
      had_previous_ = true;
      previous_ = current;
    }

    if (value.empty()) {
      ::unsetenv(name_);
      return;
    }

    ::setenv(name_, value.c_str(), 1);
  }

  ~ScopedEnvVar() {
    if (had_previous_) {
      ::setenv(name_, previous_.c_str(), 1);
      return;
    }
    ::unsetenv(name_);
  }

private:
  const char *name_;
  bool had_previous_ = false;
  std::string previous_;
};

[[nodiscard]] bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

[[nodiscard]] std::string trim_copy(std::string s) {
  while (!s.empty() && is_ascii_space(s.front())) {
    s.erase(s.begin());
  }
  while (!s.empty() && is_ascii_space(s.back())) {
    s.pop_back();
  }
  return s;
}

[[nodiscard]] std::vector<std::string> split_lines(const std::string &s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string line;
  while (std::getline(iss, line)) {
    line = trim_copy(std::move(line));
    if (!line.empty()) {
      out.push_back(std::move(line));
    }
  }
  return out;
}

[[nodiscard]] bool is_digits(const std::string &s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

[[nodiscard]] bool is_greeter_username(std::string_view u) {
  return (u == "gdm" || u == "lightdm" || u == "sddm");
}

[[nodiscard]] bool is_safe_shell_username(std::string_view u) {
  if (u.empty()) {
    return false;
  }
  for (char ch : u) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) != 0) {
      continue;
    }
    if (c == '_' || c == '-' || c == '.') {
      continue;
    }
    return false;
  }
  return true;
}

/// Выполняет команду с таймаутом и возвращает stdout (в конце без \n/\r).
/// При таймауте возвращает пустую строку.
std::string exec_command(const std::string &cmd, int timeout_seconds = 2) {
  std::array<char, 256> buffer{};
  std::string result;

  // Используем coreutils timeout для ограничения времени выполнения
  std::string timed_cmd =
      "timeout " + std::to_string(timeout_seconds) + "s " + cmd;

  FILE *pipe = popen(timed_cmd.c_str(), "r");
  if (!pipe)
    return "";

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    result += buffer.data();
  }

  int status = pclose(pipe);
  // Код 124 = timeout завершил процесс по таймауту
  if (WIFEXITED(status) && WEXITSTATUS(status) == 124) {
    std::cerr << "[punto] exec_command timeout: " << cmd << "\n";
    return "";
  }

  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }

  return result;
}

/// Парсит вывод "KEY=VALUE" в map.
[[nodiscard]] std::unordered_map<std::string, std::string>
parse_key_value_lines(const std::string &s) {
  std::unordered_map<std::string, std::string> out;
  for (const auto &line : split_lines(s)) {
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    out.emplace(std::move(key), std::move(value));
  }
  return out;
}

/// Читает переменные окружения из /proc/<pid>/environ
std::unordered_map<std::string, std::string>
read_proc_environ(const std::string &pid, uid_t expected_uid) {
  std::unordered_map<std::string, std::string> env;
  const std::string path = "/proc/" + pid + "/environ";

  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return env;
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_uid != expected_uid) {
    ::close(fd);
    return env;
  }

  std::string content;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n > 0) {
      content.append(buffer.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  ::close(fd);

  std::size_t start = 0;
  while (start < content.size()) {
    std::size_t end = content.find('\0', start);
    if (end == std::string::npos) {
      break;
    }

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

Display *open_display_from_snapshot(const X11SessionInfo &snap) {
  if (snap.display.empty()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(x11_open_mutex());
  ScopedEnvVar xauthority_override{"XAUTHORITY", snap.xauthority_path};
  return XOpenDisplay(snap.display.c_str());
}

} // namespace

bool X11Session::initialize() {
  if (initialized_.load(std::memory_order_acquire)) {
    return true;
  }

  std::optional<ActiveSession> active = find_active_session_loginctl();

  std::string username;
  std::string session_id;
  std::string leader_pid;

  if (active) {
    username = active->username;
    session_id = active->session_id;
    leader_pid = active->leader_pid;
  } else {
    auto u = find_active_user_fallback();
    if (!u) {
      std::cerr
          << "[punto] Ошибка: не удалось найти активного GUI пользователя\n";
      return false;
    }
    username = *u;
  }

  if (username.empty() || is_greeter_username(username)) {
    // Greeter (gdm/lightdm/sddm) не считаем рабочей GUI-сессией.
    return false;
  }

  struct passwd *pw = getpwnam(username.c_str());
  if (!pw) {
    std::cerr << "[punto] Ошибка: пользователь " << username
              << " не найден в системе\n";
    return false;
  }

  X11SessionInfo next;
  next.session_id = std::move(session_id);
  next.username = std::move(username);
  next.uid = pw->pw_uid;
  next.gid = pw->pw_gid;
  if (pw->pw_dir) {
    next.home_dir = pw->pw_dir;
  }
  next.xdg_runtime_dir = "/run/user/" + std::to_string(next.uid);
  if (!next.home_dir.empty()) {
    next.xdg_config_home = next.home_dir + "/.config";
  }

  bool env_ok = false;
  if (!leader_pid.empty()) {
    env_ok = find_session_env_by_pid(leader_pid, next);
  }
  if (!env_ok) {
    env_ok = find_session_env_by_user(next.username, next);
  }

  if (!env_ok || next.display.empty()) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      info_ = next;
    }
    // Не поднимаем "валидную" сессию без DISPLAY — иначе сервис прилипнет
    // к нерабочему контексту и не будет пытаться реинициализироваться.
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    info_ = std::move(next);
  }
  initialized_.store(true, std::memory_order_release);

  // Healthcheck: проверяем, что реально можем подключиться к X.
  if (!verify_x11_access()) {
    reset();
    return false;
  }

  return true;
}

X11Session::RefreshResult X11Session::refresh() {
  std::optional<ActiveSession> active = find_active_session_loginctl();

  if (!active) {
    // systemd/loginctl может быть недоступен — используем fallback.
    const bool was_valid = initialized_.load(std::memory_order_acquire);

    auto fallback_user = find_active_user_fallback();
    if (!fallback_user) {
      return RefreshResult::Unchanged;
    }

    // Если имя совпадает с текущим — просто пытаемся обновить env через
    // user-scan.
    const X11SessionInfo cur = info();
    if (was_valid && cur.username == *fallback_user) {
      X11SessionInfo next = cur;
      const bool env_ok = find_session_env_by_user(next.username, next);
      if (env_ok && !next.display.empty()) {
        const bool env_changed = (cur.display != next.display ||
                                  cur.xauthority_path != next.xauthority_path ||
                                  cur.xdg_runtime_dir != next.xdg_runtime_dir);
        if (env_changed) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            info_ = std::move(next);
          }
          if (!verify_x11_access()) {
            reset();
            return RefreshResult::Invalidated;
          }
          return RefreshResult::Updated;
        }
      }
      return RefreshResult::Unchanged;
    }

    // Имя изменилось (или ранее было невалидно) — переинициализируемся.
    reset();
    if (initialize()) {
      return RefreshResult::Updated;
    }

    // Не удалось инициализироваться — остаёмся в невалидном состоянии.
    if (was_valid) {
      return RefreshResult::Invalidated;
    }
    return RefreshResult::Unchanged;
  }

  if (active->username.empty() || is_greeter_username(active->username)) {
    const bool was_valid = initialized_.load(std::memory_order_acquire);
    if (was_valid) {
      reset();
      return RefreshResult::Invalidated;
    }
    return RefreshResult::Unchanged;
  }

  X11SessionInfo cur;
  {
    std::lock_guard<std::mutex> lock(mu_);
    cur = info_;
  }

  const bool same_session =
      (!cur.session_id.empty() && cur.session_id == active->session_id &&
       cur.username == active->username);

  // Всегда перечитываем env: DISPLAY/XAUTHORITY/XDG_RUNTIME_DIR могут
  // измениться в рамках одной логин-сессии (ранняя стадия после логина).
  X11SessionInfo next = cur;
  next.session_id = active->session_id;
  next.username = active->username;

  // Всегда обновляем uid/gid/home для текущего активного пользователя.
  struct passwd *pw = getpwnam(next.username.c_str());
  if (!pw) {
    const bool was_valid = initialized_.load(std::memory_order_acquire);
    if (was_valid) {
      reset();
      return RefreshResult::Invalidated;
    }
    return RefreshResult::Unchanged;
  }

  next.uid = pw->pw_uid;
  next.gid = pw->pw_gid;
  next.home_dir = (pw->pw_dir != nullptr) ? pw->pw_dir : std::string{};
  next.xdg_runtime_dir = "/run/user/" + std::to_string(next.uid);
  if (!next.home_dir.empty()) {
    next.xdg_config_home = next.home_dir + "/.config";
  }

  bool env_ok = false;
  if (!active->leader_pid.empty()) {
    env_ok = find_session_env_by_pid(active->leader_pid, next);
  }
  if (!env_ok) {
    env_ok = find_session_env_by_user(next.username, next);
  }

  if (!env_ok || next.display.empty()) {
    const bool was_valid = initialized_.load(std::memory_order_acquire);
    {
      std::lock_guard<std::mutex> lock(mu_);
      info_ = next;
    }
    initialized_.store(false, std::memory_order_release);
    if (was_valid) {
      return RefreshResult::Invalidated;
    }
    return RefreshResult::Unchanged;
  }

  const bool env_changed =
      (cur.display != next.display ||
       cur.xauthority_path != next.xauthority_path ||
       cur.xdg_runtime_dir != next.xdg_runtime_dir ||
       cur.username != next.username || cur.session_id != next.session_id);

  if (!env_changed && same_session &&
      initialized_.load(std::memory_order_acquire)) {
    return RefreshResult::Unchanged;
  }

  // Применяем обновление атомарно.
  {
    std::lock_guard<std::mutex> lock(mu_);
    info_ = std::move(next);
  }
  initialized_.store(true, std::memory_order_release);

  if (!verify_x11_access()) {
    reset();
    return RefreshResult::Invalidated;
  }

  return RefreshResult::Updated;
}

void X11Session::reset() noexcept {
  initialized_.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lock(mu_);
    info_ = X11SessionInfo{};
  }
}

bool X11Session::is_valid() const noexcept {
  return initialized_.load(std::memory_order_acquire);
}

X11SessionInfo X11Session::info() const {
  std::lock_guard<std::mutex> lock(mu_);
  return info_;
}

bool X11Session::is_wayland_session() const {
  std::lock_guard<std::mutex> lock(mu_);
  return !info_.wayland_display.empty();
}

Display *X11Session::open_display() const {
  if (!initialized_.load(std::memory_order_acquire)) {
    return nullptr;
  }

  X11SessionInfo snap;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snap = info_;
  }

  return open_display_from_snapshot(snap);
}

std::optional<X11Session::ActiveSession>
X11Session::find_active_session_loginctl() {
  const std::string out =
      exec_command("loginctl list-sessions --no-legend --no-pager 2>/dev/null");
  if (out.empty()) {
    return std::nullopt;
  }

  for (const auto &line : split_lines(out)) {
    std::istringstream iss(line);
    std::string sid;
    std::string uid;
    std::string user;
    std::string seat;

    iss >> sid >> uid >> user >> seat;
    if (sid.empty() || user.empty() || seat.empty()) {
      continue;
    }
    if (seat != "seat0") {
      continue;
    }
    if (!is_digits(sid)) {
      continue;
    }

    const std::string show = exec_command(
        "loginctl show-session " + sid +
        " -p Active -p Class -p Name -p Leader --no-pager 2>/dev/null");
    if (show.empty()) {
      continue;
    }

    auto kv = parse_key_value_lines(show);

    const auto it_active = kv.find("Active");
    const auto it_class = kv.find("Class");
    const auto it_name = kv.find("Name");
    const auto it_leader = kv.find("Leader");

    const std::string active = (it_active != kv.end()) ? it_active->second : "";
    const std::string cls = (it_class != kv.end()) ? it_class->second : "";
    const std::string name = (it_name != kv.end()) ? it_name->second : user;
    const std::string leader = (it_leader != kv.end()) ? it_leader->second : "";

    if (active != "yes") {
      continue;
    }
    if (cls != "user") {
      // Greeter/manager сессии не используем.
      continue;
    }
    if (name.empty() || is_greeter_username(name)) {
      continue;
    }

    ActiveSession s;
    s.session_id = sid;
    s.username = name;
    if (is_digits(leader)) {
      s.leader_pid = leader;
    }
    return s;
  }

  return std::nullopt;
}

std::optional<std::string> X11Session::find_active_user_fallback() {
  // Метод 1: who
  std::string user = exec_command(
      "who 2>/dev/null | grep '(:0)' | awk '{print $1}' | head -n 1");
  if (!user.empty() && !is_greeter_username(user) && user != "root") {
    return user;
  }

  // Метод 2: владелец /dev/tty1 или консоли
  user = exec_command("stat -c '%U' /dev/tty1 2>/dev/null");
  if (!user.empty() && user != "root" && !is_greeter_username(user)) {
    return user;
  }

  return std::nullopt;
}

bool X11Session::find_session_env_by_pid(const std::string &pid,
                                         X11SessionInfo &out) {
  if (!is_digits(pid)) {
    return false;
  }

  auto env = read_proc_environ(pid, static_cast<uid_t>(out.uid));
  if (env.empty()) {
    return false;
  }

  if (auto it = env.find("DISPLAY"); it != env.end()) {
    out.display = it->second;
  }

  if (auto it = env.find("XAUTHORITY"); it != env.end()) {
    out.xauthority_path = it->second;
  }

  if (auto it = env.find("XDG_RUNTIME_DIR"); it != env.end()) {
    out.xdg_runtime_dir = it->second;
  }

  if (auto it = env.find("XDG_CONFIG_HOME"); it != env.end()) {
    out.xdg_config_home = it->second;
  }

  if (auto it = env.find("WAYLAND_DISPLAY"); it != env.end()) {
    out.wayland_display = it->second;
  }

  return !out.display.empty();
}

bool X11Session::find_session_env_by_user(const std::string &username,
                                          X11SessionInfo &out) {
  if (username.empty() || is_greeter_username(username)) {
    return false;
  }
  if (!is_safe_shell_username(username)) {
    return false;
  }

  // Ищем процесс gnome-session или KDE пользователя.
  std::string pid = exec_command("pgrep -u " + username +
                                 " 'gnome-session|plasma|plasmashell' "
                                 "2>/dev/null | head -n 1");

  if (pid.empty()) {
    // Fallback: gnome-shell (в т.ч. Wayland)
    pid = exec_command("pgrep -u " + username +
                       " -f '^/usr/bin/gnome-shell' 2>/dev/null | head -n 1");
  }

  if (pid.empty()) {
    return false;
  }

  return find_session_env_by_pid(pid, out);
}

bool X11Session::verify_x11_access() const {
  if (!initialized_.load(std::memory_order_acquire)) {
    return false;
  }

  Display *display = open_display();

  bool ok = false;
  if (display) {
    // Минимальная операция, чтобы убедиться, что соединение реально живое.
    XkbStateRec state;
    ok = (XkbGetState(display, XkbUseCoreKbd, &state) == Success);
    XCloseDisplay(display);
  }

  return ok;
}

int X11Session::get_current_keyboard_layout() const {
  if (!initialized_.load(std::memory_order_acquire)) {
    return -1;
  }

  Display *display = open_display();

  int group = -1;
  if (display) {
    XkbStateRec state;
    if (XkbGetState(display, XkbUseCoreKbd, &state) == Success) {
      group = state.group;
    }
    XCloseDisplay(display);
  }

  return group;
}

bool X11Session::set_keyboard_layout(int index) const {
  if (!initialized_.load(std::memory_order_acquire)) {
    return false;
  }

  Display *display = open_display();

  bool success = false;
  if (display) {
    XkbLockGroup(display, XkbUseCoreKbd, static_cast<unsigned int>(index));
    XSync(display, False);

    // Проверяем результат в том же соединении с несколькими попытками.
    // XkbLockGroup может применяться с небольшой задержкой.
    constexpr int kMaxRetries = 5;
    constexpr int kRetryDelayUs = 1000; // 1 мс

    for (int retry = 0; retry < kMaxRetries; ++retry) {
      XkbStateRec state;
      if (XkbGetState(display, XkbUseCoreKbd, &state) == Success) {
        if (static_cast<int>(state.group) == index) {
          success = true;
          break;
        }
      }

      if (retry < kMaxRetries - 1) {
        usleep(kRetryDelayUs);
        XSync(display, False);
      }
    }

    XCloseDisplay(display);
  }

  return success;
}

void X11Session::start_background_refresh() {
  std::lock_guard<std::mutex> lock(refresh_mutex_);

  // Если уже есть активный refresh, ничего не делаем
  if (pending_refresh_.valid()) {
    // Проверяем, не завершился ли он
    if (pending_refresh_.wait_for(std::chrono::milliseconds{0}) !=
        std::future_status::ready) {
      return; // Еще выполняется
    }
    // Завершился, но результат не забрали - игнорируем старый результат
    pending_refresh_ = {};
  }

  pending_refresh_ = std::async(std::launch::async, [this]() {
    return this->refresh();
  });
}

std::optional<X11Session::RefreshResult> X11Session::poll_refresh_result() {
  std::lock_guard<std::mutex> lock(refresh_mutex_);

  if (!pending_refresh_.valid()) {
    return std::nullopt;
  }

  if (pending_refresh_.wait_for(std::chrono::milliseconds{0}) ==
      std::future_status::ready) {
    return pending_refresh_.get();
  }

  return std::nullopt;
}

} // namespace punto
